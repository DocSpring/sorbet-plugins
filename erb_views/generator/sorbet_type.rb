# frozen_string_literal: true

module ErbViews
  # A type as Sorbet prints it (`T.nilable(T::Array[String])`, `Integer(1)`, `[Integer, String] (2-tuple)`,
  # `{a: Integer(1)} (shape of T::Hash[T.untyped, T.untyped])`), parsed so the generator can widen it into a type an
  # RBI declares: literals become their class, tuples arrays and shapes hashes.
  module SorbetType
    # Nodes: [:name, name, args or nil], [:literal, class name], [:nilable, t], [:any, ts], [:all, ts],
    # [:class_of, t], [:tuple, ts], [:shape, [[key, value]]], [:raw, text], [:untyped], [:noreturn].
    UNTYPED = [:untyped].freeze

    # The relation classes Tapioca generates under each model, whose elements are the model.
    RELATION_SUFFIX = /::(Private(?:Association)?Relation(?:WhereChain)?|PrivateCollectionProxy)\z/

    class Parser
      def initialize(text)
        @text = text
        @at = 0
      end

      def parse
        type = parse_type
        raise ArgumentError, "unexpected #{rest.inspect}" unless @at == @text.size

        type
      end

      private

      def parse_type
        type = if take('T.nilable(') then [:nilable, finish(parse_type, ')')]
               elsif take('T.any(') then [:any, list(')')]
               elsif take('T.all(') then [:all, list(')')]
               elsif take('T.class_of(') then [:class_of, finish(parse_type, ')')]
               elsif take('T.untyped') then UNTYPED
               elsif take('T.noreturn') then [:noreturn]
               elsif take('T.self_type') || take('T.attached_class') then UNTYPED
               elsif take('T.type_parameter(') then skip_balanced(')') && UNTYPED
               elsif rest.start_with?('T.proc') then [:raw, proc_text]
               elsif take('[') then [:tuple, list(']')]
               elsif take('{') then [:shape, entries]
               else name
               end
        skip_annotation
        type
      end

      def name
        constant = rest[/\A[A-Z]\w*(?:::[A-Z]\w*)*/] or raise ArgumentError, "no type at #{rest.inspect}"
        @at += constant.size
        if take('[') then [:name, constant, list(']')]
        elsif rest.start_with?('(') then skip_balanced_from_open && [:literal, constant]
        else [:name, constant, nil]
        end
      end

      # `{a: Integer, String("b") => Integer}`
      def entries
        found = []
        return found if take('}')

        loop do
          key = if (symbol = rest[/\A[a-z_]\w*[?!]?: /])
                  @at += symbol.size
                  [:name, 'Symbol', nil]
                else
                  parsed = parse_type
                  raise ArgumentError, "expected => at #{rest.inspect}" unless take(' => ')

                  parsed
                end
          found << [key, parse_type]
          break if take('}')
          raise ArgumentError, "expected , at #{rest.inspect}" unless take(', ')
        end
        found
      end

      def list(close)
        found = []
        return found if take(close)

        loop do
          found << parse_type
          break if take(close)
          raise ArgumentError, "expected , at #{rest.inspect}" unless take(', ')
        end
        found
      end

      def finish(value, close)
        raise ArgumentError, "expected #{close} at #{rest.inspect}" unless take(close)

        value
      end

      # ` (2-tuple)` and ` (shape of T::Hash[...])` after a tuple or shape.
      def skip_annotation
        return unless rest.match?(/\A \((\d+-tuple|shape of )/)

        @at += 1
        skip_balanced_from_open
      end

      # `T.proc.params(a: String).returns(Integer)`: kept as Sorbet prints it.
      def proc_text
        start = @at
        @at += 'T.proc'.size
        while (segment = rest[/\A\.\w+/])
          @at += segment.size
          skip_balanced_from_open if rest.start_with?('(')
        end
        @text[start...@at]
      end

      def skip_balanced_from_open
        raise ArgumentError, "expected ( at #{rest.inspect}" unless take('(')

        skip_balanced(')')
      end

      # Skips to the `)` that closes one already taken.
      def skip_balanced(_close)
        depth = 1
        while depth.positive?
          raise ArgumentError, 'unbalanced' if @at >= @text.size

          char = @text[@at]
          depth += 1 if '([{'.include?(char)
          depth -= 1 if ')]}'.include?(char)
          @at += 1
        end
        true
      end

      def take(prefix)
        return false unless rest.start_with?(prefix)

        @at += prefix.size
        true
      end

      def rest = @text[@at..]
    end

    module_function

    # nil when Sorbet printed something the parser doesn't know.
    def parse(text)
      Parser.new(text).parse
    rescue ArgumentError
      nil
    end

    # The type an RBI declares for a value of this type: literals widened, tuples as arrays, shapes as hashes, and
    # unions simplified.
    def widen(type)
      case type[0]
      when :literal then [:name, type[1], nil]
      when :name
        return [:name, 'T::Boolean', nil] if %w[TrueClass FalseClass].include?(type[1])

        [:name, type[1], type[2]&.map { |arg| widen_argument(arg) }]
      when :nilable then union([widen(type[1]), [:name, 'NilClass', nil]])
      when :any then union(type[1].map { |member| widen(member) })
      when :all then [:all, type[1].map { |member| widen(member) }]
      when :class_of then [:class_of, widen(type[1])]
      when :tuple then [:name, 'T::Array', [type[1].empty? ? UNTYPED : union(type[1].map { |member| widen(member) })]]
      when :shape
        return [:name, 'T::Hash', [UNTYPED, UNTYPED]] if type[1].empty?

        [:name, 'T::Hash', [union(type[1].map { |key, _| widen(key) }), union(type[1].map { |_, value| widen(value) })]]
      else type
      end
    end

    # A type argument keeps its tuples (`T::Array[[OnsiteLicenseContact, String]]`), so a block that takes the pair
    # apart (`|contact, email|`) types each part.
    def widen_argument(type)
      return [:tuple, type[1].map { |member| widen_argument(member) }] if type[0] == :tuple && !type[1].empty?

      widen(type)
    end

    # A union of widened types: nested unions flattened, duplicates dropped, TrueClass and FalseClass as T::Boolean,
    # NilClass as T.nilable, T.untyped absorbing the rest, and three or more classes with a common superclass (other
    # than Object) as that superclass.
    def union(types)
      members = types.flat_map { |type| type[0] == :any ? type[1] : [type] }.flat_map do |type|
        type[0] == :nilable ? [type[1], [:name, 'NilClass', nil]] : [type]
      end
      members = members.reject { |type| type[0] == :noreturn }
      return UNTYPED if members.any? { |type| type == UNTYPED }

      nilable = members.reject! { |type| type == [:name, 'NilClass', nil] }
      booleans = members.select { |type| [[:name, 'TrueClass', nil], [:name, 'FalseClass', nil], [:name, 'T::Boolean', nil]].include?(type) }
      members = (members - booleans) + (booleans.empty? ? [] : [[:name, 'T::Boolean', nil]])
      members = members.uniq { |type| render(type) }
      members = common_superclass(members) || members
      inner = members.size == 1 ? members.first : [:any, members]
      return [:name, 'NilClass', nil] if members.empty? && nilable
      return [:noreturn] if members.empty?

      nilable ? [:nilable, inner] : inner
    end

    def common_superclass(members)
      return nil if members.size < 3 || !members.all? { |type| type[0] == :name && type[2].nil? }

      classes = members.map { |type| Object.const_get(type[1]) }
      return nil unless classes.all?(Class)

      common = classes.map { |klass| klass.ancestors.grep(Class) }.reduce(:&).first
      common && ![Object, BasicObject].include?(common) && common.name ? [[:name, common.name, nil]] : nil
    rescue NameError
      nil
    end

    # What a collection of this type yields: an array's or set's element, or a model for its relations.
    def element(type)
      case type[0]
      when :nilable then element(type[1])
      when :any then union(type[1].map { |member| element(member) })
      when :name
        if %w[T::Array T::Set T::Enumerable T::Enumerator].include?(type[1]) && type[2]&.size == 1
          type[2][0]
        elsif type[1].match?(RELATION_SUFFIX)
          [:name, type[1].sub(RELATION_SUFFIX, ''), nil]
        else
          UNTYPED
        end
      else UNTYPED
      end
    end

    def strip_nil(type)
      return type[1] if type[0] == :nilable

      type
    end

    # Whether every value of `sub` is a `sup`: each member of its union is a member of `sup`'s or a subclass of one.
    def subtype?(sub, sup)
      return true if sup == UNTYPED
      return false if sub == UNTYPED

      supers = members(sup)
      members(sub).all? { |member| supers.any? { |super_member| member_subtype?(member, super_member) } }
    end

    def members(type)
      case type[0]
      when :nilable then members(type[1]) + [[:name, 'NilClass', nil]]
      when :any then type[1].flat_map { |member| members(member) }
      else [type]
      end
    end

    def member_subtype?(member, super_member)
      return true if render(member) == render(super_member)
      return false unless member[0] == :name && super_member[0] == :name && member[2].nil? && super_member[2].nil?

      klass = Object.const_get(member[1])
      parent = Object.const_get(super_member[1])
      klass.is_a?(Module) && parent.is_a?(Module) && (klass <= parent || false)
    rescue NameError
      false
    end

    def render(type)
      case type[0]
      when :untyped then 'T.untyped'
      when :noreturn then 'T.noreturn'
      when :name, :literal then type[2] ? "#{type[1]}[#{type[2].map { |arg| render(arg) }.join(', ')}]" : type[1]
      when :nilable then "T.nilable(#{render(type[1])})"
      when :any then "T.any(#{type[1].map { |member| render(member) }.join(', ')})"
      when :all then "T.all(#{type[1].map { |member| render(member) }.join(', ')})"
      when :class_of then "T.class_of(#{render(type[1])})"
      when :tuple then "[#{type[1].map { |member| render(member) }.join(', ')}]"
      when :raw then type[1]
      else raise ArgumentError, "can't render #{type.inspect}"
      end
    end
  end
end
