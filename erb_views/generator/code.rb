# frozen_string_literal: true

require 'prism'

module ErbViews
  # What a method or a template does that the views care about, read with Prism: the instance variables it assigns,
  # the methods it calls on itself, and its `render` calls. Each one is definite (it runs whenever the code does) or
  # conditional (inside an `if`, a block, the right of `&&`, after an early `return`...).
  module Code
    # `file` is relative to the app. `scope` is the code the node was found in (a method's body, a block, a template),
    # where its local variables are assigned.
    IvarWrite = Struct.new(:name, :file, :node, :value, :definite, keyword_init: true)
    SelfCall = Struct.new(:name, :file, :node, :scope, :definite, keyword_init: true)
    # `bindings`: when the render is in a method the graph followed from a call, the method's parameters =>
    # [the call's argument, the caller's scope] (`render template` in `render_page(:new, 'Title')`). `via`: the
    # SelfCalls that lead to it, outermost first, and `chain_writes`: the ivars the outermost call's method assigns.
    # `dominating`: the ivars its own method assigns on every path to it, so a write and a render in the same block
    # (`format.html { @rows = load_rows; render :index }`) hand over a value even though the block may not run.
    Render = Struct.new(:file, :node, :scope, :definite, :bindings, :via, :chain_writes, :dominating, keyword_init: true)
    # A call that ends the response (`redirect_to`, `head`, `send_data`...), so no template renders after it.
    Respond = Struct.new(:name, :definite, keyword_init: true)

    # `local_keys`: the locals a template reads through `local_assigns` (`local_assigns[:meta]`), which callers may
    # leave out.
    Facts = Struct.new(:writes, :calls, :renders, :responds, :local_keys, keyword_init: true) do
      def self.empty = new(writes: [], calls: [], renders: [], responds: [], local_keys: [])
    end

    # A SelfCall's name for `super`.
    SUPER = :'(super)'

    # (`respond_with` renders the action's template for a GET, so it isn't one.)
    RESPONDS = %i[redirect_to redirect_back redirect_back_or_to head send_data send_file].freeze

    class Scanner < Prism::Visitor
      attr_reader :facts

      def initialize(file)
        super()
        @file = file
        @facts = Facts.empty
        @definite = true
        @dominating = Set.new
      end

      def scan(node)
        @scope = node
        visit(node)
        facts
      end

      def visit_def_node(node) = nil # a method defined inside: not this code
      def visit_lambda_node(node) = conditional { super }

      def visit_statements_node(node)
        parent = @definite
        definite = parent
        node.body.each do |statement|
          @definite = definite
          visit(statement)
          # After `return if ...` (or any statement with a `return` in it), the rest may not run.
          definite = false if exits?(statement)
        end
        @definite = parent
      end

      # Each branch on its own: a write in one doesn't reach a render in another.
      def visit_if_node(node)
        visit(node.predicate)
        conditional { visit(node.statements) }
        conditional { visit(node.subsequent) }
      end

      def visit_unless_node(node)
        visit(node.predicate)
        conditional { visit(node.statements) }
        conditional { visit(node.else_clause) }
      end

      def visit_case_node(node)
        visit(node.predicate)
        node.conditions.each { |condition| conditional { visit(condition) } }
        conditional { visit(node.else_clause) }
      end

      def visit_case_match_node(node)
        visit(node.predicate)
        node.conditions.each { |condition| conditional { visit(condition) } }
        conditional { visit(node.else_clause) }
      end

      def visit_and_node(node)
        visit(node.left)
        conditional { visit(node.right) }
      end

      def visit_or_node(node)
        visit(node.left)
        conditional { visit(node.right) }
      end

      def visit_while_node(node)
        visit(node.predicate)
        conditional { visit(node.statements) }
      end

      def visit_until_node(node)
        visit(node.predicate)
        conditional { visit(node.statements) }
      end

      def visit_for_node(node)
        visit(node.collection)
        conditional { visit(node.statements) }
      end

      def visit_block_node(node) = conditional { super }
      def visit_rescue_node(node) = conditional { super }
      def visit_rescue_modifier_node(node)
        before = @dominating.dup
        visit(node.expression)
        conditional do
          @dominating = before # the expression may have raised before its writes
          visit(node.rescue_expression)
        end
      end

      # A rescue clause can start before any of the body's writes, and an `else` clause runs only when the body
      # didn't raise. An `ensure` clause always runs, after a body that may have raised anywhere.
      def visit_begin_node(node)
        before = @dominating.dup
        visit(node.statements)
        finished = @dominating
        if node.rescue_clause
          @dominating = before.dup
          visit(node.rescue_clause)
          conditional do
            @dominating = finished.dup
            visit(node.else_clause)
          end
          finished = before
        end
        @dominating = before.dup
        visit(node.ensure_clause)
        @dominating |= finished
      end

      # `super` runs the ancestor's method: a self call the render graph resolves from the method it's in.
      def visit_super_node(node)
        @facts.calls << SelfCall.new(name: SUPER, file: @file, node:, scope: @scope, definite: @definite)
        super
      end

      def visit_forwarding_super_node(node)
        @facts.calls << SelfCall.new(name: SUPER, file: @file, node:, scope: @scope, definite: @definite)
        super
      end

      def visit_instance_variable_write_node(node)
        write(node.name, node, node.value)
        super
        @dominating << node.name.to_s
      end

      def visit_instance_variable_or_write_node(node)
        write(node.name, node, node.value)
        super
        @dominating << node.name.to_s
      end

      def visit_instance_variable_operator_write_node(node)
        write(node.name, node, nil)
        super
        @dominating << node.name.to_s
      end

      def visit_instance_variable_and_write_node(node)
        # `@a &&= x` assigns only when @a is already set.
        conditional { write(node.name, node, node.value) }
        super
      end

      def visit_instance_variable_target_node(node)
        write(node.name, node, nil) # `@a, @b = ...`
        super
        @dominating << node.name.to_s
      end

      def visit_call_node(node)
        if %i[[] fetch key? include?].include?(node.name) && local_assigns?(node.receiver)
          key = node.arguments&.arguments&.first
          @facts.local_keys << key.unescaped if key.is_a?(Prism::SymbolNode)
        end
        if node.receiver.nil? || node.receiver.is_a?(Prism::SelfNode)
          case node.name
          when :render
            @facts.renders << Render.new(file: @file, node:, scope: @scope, definite: @definite, dominating: @dominating.dup)
          when :instance_variable_set
            name = node.arguments&.arguments&.first
            if name.is_a?(Prism::SymbolNode)
              write(name.unescaped.to_sym, node, node.arguments.arguments[1])
              @dominating << name.unescaped
            end
          when *RESPONDS
            @facts.responds << Respond.new(name: node.name, definite: @definite)
          end
          @facts.calls << SelfCall.new(name: node.name, file: @file, node:, scope: @scope, definite: @definite)
        end
        # The receiver and arguments run whenever the call does; a block may not run.
        visit(node.receiver)
        visit(node.arguments)
        visit(node.block)
      end

      private

      def local_assigns?(node) = node.is_a?(Prism::CallNode) && node.name == :local_assigns && node.receiver.nil?

      def write(name, node, value)
        @facts.writes << IvarWrite.new(name: name.to_s, file: @file, node:, value:, definite: @definite)
      end

      # Code that may not run: its writes aren't definite, and don't dominate what comes after it.
      def conditional
        definite = @definite
        dominating = @dominating
        @definite = false
        @dominating = dominating.dup
        yield
      ensure
        @definite = definite
        @dominating = dominating
      end

      def exits?(node)
        return true if node.is_a?(Prism::ReturnNode)
        return false if node.is_a?(Prism::DefNode) || node.is_a?(Prism::LambdaNode) || node.is_a?(Prism::BlockNode)

        node.compact_child_nodes.any? { |child| exits?(child) }
      end
    end

    module_function

    def scan(node, file) = Scanner.new(file).scan(node)

    CONTINUES = Set[:continue].freeze

    # The calls on self that are statements in a method's body (not inside a block, a lambda or another method):
    # the ones the type probe can put a `return` before.
    def statement_calls(node, found = [])
      case node
      when Prism::DefNode then statement_calls(node.body, found) if found.empty? && node.body
      when Prism::BlockNode, Prism::LambdaNode then nil
      when Prism::StatementsNode
        node.body.each do |statement|
          if statement.is_a?(Prism::CallNode) && (statement.receiver.nil? || statement.receiver.is_a?(Prism::SelfNode))
            found << statement
          end
          statement_calls(statement, found)
        end
      when Prism::Node then node.compact_child_nodes.each { |child| statement_calls(child, found) unless child.is_a?(Prism::DefNode) }
      end
      found
    end

    # The `return`s in a method's body (not in a block, a lambda or another method), except `return if performed?`'s:
    # the action rendered or redirected, so nothing more renders after it.
    def returns(node, found = [])
      found << node if node.is_a?(Prism::ReturnNode)
      node.compact_child_nodes.each do |child|
        next if child.is_a?(Prism::DefNode) || child.is_a?(Prism::BlockNode) || child.is_a?(Prism::LambdaNode)
        next if node.is_a?(Prism::IfNode) && child.equal?(node.statements) && performed?(node.predicate)

        returns(child, found)
      end
      found
    end

    # `performed?`: whether the action has rendered or redirected.
    def performed?(node)
      node.is_a?(Prism::CallNode) && node.name == :performed? && (node.receiver.nil? || node.receiver.is_a?(Prism::SelfNode))
    end

    # The instance variables a method reads or assigns.
    def ivar_names(node, found = Set.new)
      case node
      when Prism::InstanceVariableReadNode, Prism::InstanceVariableWriteNode, Prism::InstanceVariableTargetNode,
           Prism::InstanceVariableOrWriteNode, Prism::InstanceVariableAndWriteNode, Prism::InstanceVariableOperatorWriteNode
        found << node.name.to_s
      end
      node.compact_child_nodes.each { |child| ivar_names(child, found) unless child.is_a?(Prism::DefNode) } if node.is_a?(Prism::Node)
      found
    end

    # How the paths through some code can end: `:ends` (it rendered, responded or raised), `:returns` (a `return`
    # before any of those) or `:continue` (it ran to the end). `self_call` gives a call to the renderer's own method
    # its outcomes. An action whose method can't `:continue` or `:return` never renders its default template.
    def outcomes(node, &self_call)
      case node
      when Prism::StatementsNode then sequence(node.body, &self_call)
      when Prism::BeginNode
        found = outcomes(node.statements, &self_call)
        node.rescue_clause ? found | outcomes(node.rescue_clause.statements, &self_call) : found
      when Prism::ParenthesesNode then outcomes(node.body, &self_call)
      when Prism::IfNode
        subsequent = node.subsequent.is_a?(Prism::ElseNode) ? node.subsequent.statements : node.subsequent
        found = node.statements ? outcomes(node.statements, &self_call) : CONTINUES
        # `return if performed?` returns after a render or a redirect.
        found = found.to_set { |outcome| outcome == :returns ? :ends : outcome } if performed?(node.predicate)
        found | (subsequent ? outcomes(subsequent, &self_call) : CONTINUES)
      when Prism::UnlessNode then either(node.statements, node.else_clause&.statements, &self_call)
      when Prism::CaseNode, Prism::CaseMatchNode
        branches = node.conditions.map { |branch| branch.statements } + [node.else_clause&.statements]
        branches.map { |branch| branch ? outcomes(branch, &self_call) : CONTINUES }.reduce(:|)
      when Prism::AndNode, Prism::OrNode then sequence([node.left], CONTINUES | outcomes(node.right, &self_call), &self_call)
      when Prism::ReturnNode
        node.arguments && sequence(node.arguments.arguments, &self_call).include?(:ends) ? Set[:ends] : Set[:returns]
      when Prism::CallNode then call_outcomes(node, &self_call)
      else CONTINUES
      end
    end

    # The statements in order: the paths that continue past one run the next.
    def sequence(statements, tail = nil, &self_call)
      found = CONTINUES
      statements.each do |statement|
        break unless found.include?(:continue)

        found = (found - CONTINUES) | outcomes(statement, &self_call)
      end
      tail && found.include?(:continue) ? (found - CONTINUES) | tail : found
    end

    def either(first, second, &self_call)
      (first ? outcomes(first, &self_call) : CONTINUES) | (second ? outcomes(second, &self_call) : CONTINUES)
    end

    def call_outcomes(node, &self_call)
      return CONTINUES unless node.receiver.nil? || node.receiver.is_a?(Prism::SelfNode)
      return Set[:ends] if %i[render raise fail].include?(node.name) || RESPONDS.include?(node.name)

      self_call ? self_call.call(node.name) : CONTINUES
    end

    # A literal string or symbol's value, or nil.
    def literal(node)
      case node
      when Prism::StringNode then node.unescaped
      when Prism::SymbolNode then node.unescaped
      end
    end

    # NUL stands for the part of a string we can't know.
    UNKNOWN = "\0"

    # The strings an expression can be, as far as reading the code tells: literals, interpolations (an unknown part
    # becomes UNKNOWN), the branches of an `if` or `case`, the values assigned to a local variable in `scope` or that
    # it takes as a block's parameter over a constant list (`TABS.each do |tab|`), and what a method returns
    # (`method_body` gives a self call's [body, scope], or nil). nil when it isn't a string we can follow, as with
    # `render @record` or another block's parameter.
    def string_values(node, scope, depth = 0, bindings = nil, &method_body)
      return nil if node.nil? || depth > 6

      if node.is_a?(Prism::LocalVariableReadNode) && bindings&.key?(node.name)
        argument, caller = bindings.fetch(node.name)
        return string_values(argument, caller, depth + 1, &method_body)
      end

      values = case node
               when Prism::StringNode, Prism::SymbolNode then [node.unescaped]
               when Prism::InterpolatedStringNode, Prism::InterpolatedSymbolNode
                 node.parts.reduce(['']) do |prefixes, part|
                   inner = part.is_a?(Prism::StringNode) ? [part.unescaped] : embedded_values(part, scope, depth, &method_body)
                   prefixes.product(inner || [UNKNOWN]).map(&:join)
                 end
               when Prism::LocalVariableReadNode
                 writes = local_writes(scope, node.name)
                 found = writes.map { |value| string_values(value, scope, depth + 1, &method_body) } +
                         local_block_values(scope, node.name)
                 found.empty? || found.any?(&:nil?) ? nil : found.flatten
               when Prism::ParenthesesNode then string_values(node.body, scope, depth + 1, &method_body)
               when Prism::StatementsNode then string_values(node.body.last, scope, depth + 1, &method_body)
               when Prism::IfNode, Prism::UnlessNode
                 branches = [node.statements, node.is_a?(Prism::IfNode) ? node.subsequent : node.else_clause]
                 union(branches.map { |branch| string_values(branch, scope, depth + 1, &method_body) })
               when Prism::ElseNode then string_values(node.statements, scope, depth + 1, &method_body)
               when Prism::CaseNode
                 branches = node.conditions.map(&:statements) + [node.else_clause]
                 union(branches.map { |branch| string_values(branch, scope, depth + 1, &method_body) })
               when Prism::CallNode
                 called = method_body.call(node.name) if method_body && node.receiver.nil?
                 string_values(called[0], called[1], depth + 1, &method_body) if called
               end
      return nil if values.nil? || values.size > 50

      values.uniq
    end

    # The keys a hash expression has, as far as reading the code tells: a literal's keys, the keys of the hashes it
    # splats, a local's values and the keys it's given (`options[:label] = x`), and what a lambda in a local returns
    # (`color.call('x', a: 1)` with `color = ->(key, **options) { { key:, **options } }` has key and a). Unknown
    # splats add nothing. With `index: false`, without the keys given by index (which may be given only sometimes).
    def hash_keys(node, scope, depth = 0, bindings = nil, index: true)
      return [] if node.nil? || depth > 6

      case node
      when Prism::HashNode, Prism::KeywordHashNode
        node.elements.flat_map do |element|
          case element
          when Prism::AssocNode then [literal(element.key)].compact
          when Prism::AssocSplatNode then hash_keys(element.value, scope, depth + 1, bindings, index:)
          else []
          end
        end
      when Prism::LocalVariableReadNode
        if bindings&.key?(node.name)
          argument, caller = bindings.fetch(node.name)
          hash_keys(argument, caller, depth + 1, index:)
        else
          local_writes(scope, node.name).flat_map { |value| hash_keys(value, scope, depth + 1, bindings, index:) } +
            (index ? local_index_keys(scope, node.name) : [])
        end
      when Prism::ParenthesesNode then hash_keys(node.body, scope, depth + 1, bindings, index:)
      when Prism::StatementsNode then hash_keys(node.body.last, scope, depth + 1, bindings, index:)
      when Prism::CallNode
        if %i[call ()].include?(node.name) && node.receiver.is_a?(Prism::LocalVariableReadNode)
          local_writes(scope, node.receiver.name).flat_map { |value| lambda_keys(value, node, scope, depth, index:) }
        else
          []
        end
      else []
      end.uniq
    end

    # The keys a lambda returns when `call` calls it: its keyword-rest parameter is the call's keywords.
    def lambda_keys(lambda, call, scope, depth, index: true)
      lambda = lambda.block if lambda.is_a?(Prism::CallNode) && lambda.name == :lambda && lambda.receiver.nil?
      return [] unless (lambda.is_a?(Prism::LambdaNode) || lambda.is_a?(Prism::BlockNode)) && lambda.body

      parameters = lambda.parameters&.parameters
      keyrest = parameters&.keyword_rest
      keywords = call.arguments&.arguments&.find { |argument| argument.is_a?(Prism::KeywordHashNode) }
      bindings = keyrest.respond_to?(:name) && keyrest.name && keywords ? { keyrest.name => [keywords, scope] } : {}
      hash_keys(lambda.body, scope, depth + 1, bindings, index:)
    end

    def embedded_values(part, scope, depth, &method_body)
      return nil unless part.is_a?(Prism::EmbeddedStatementsNode) && part.statements&.body&.size == 1

      string_values(part.statements.body.first, scope, depth + 1, &method_body)
    end

    def union(values) = values.any?(&:nil?) ? nil : values.flatten

    # The values assigned to a local variable anywhere in `scope` (not in methods defined inside it).
    def local_writes(scope, name)
      @local_writes ||= {}.compare_by_identity
      writes = @local_writes[scope] ||= begin
        found = Hash.new { |hash, key| hash[key] = [] }
        stack = [scope]
        until stack.empty?
          node = stack.pop
          next if node.is_a?(Prism::DefNode) && !node.equal?(scope)

          found[node.name] << node.value if node.is_a?(Prism::LocalVariableWriteNode) || node.is_a?(Prism::LocalVariableOrWriteNode)
          stack.concat(node.compact_child_nodes)
        end
        found
      end
      writes.fetch(name, [])
    end

    ITERATORS = %i[each each_with_index map flat_map filter_map select each_slice].freeze

    # For each block in `scope` whose first parameter is `name`: the strings the parameter takes, when the block
    # iterates a constant that holds strings or symbols (`TABS.each do |tab|`, read from the running app), or nil.
    def local_block_values(scope, name)
      found = []
      stack = [scope]
      until stack.empty?
        node = stack.pop
        next if node.is_a?(Prism::DefNode) && !node.equal?(scope)

        stack.concat(node.compact_child_nodes)
        next unless node.is_a?(Prism::CallNode) && node.block.is_a?(Prism::BlockNode)

        first = node.block.parameters&.parameters&.requireds&.first
        next unless first.is_a?(Prism::RequiredParameterNode) && first.name == name

        found << constant_strings(node)
      end
      found
    end

    def constant_strings(call)
      return nil unless ITERATORS.include?(call.name) && (call.receiver.is_a?(Prism::ConstantReadNode) || call.receiver.is_a?(Prism::ConstantPathNode))

      value = Object.const_get(call.receiver.slice.delete_prefix('::'))
      value.map(&:to_s) if value.is_a?(Array) && value.all? { |item| item.is_a?(String) || item.is_a?(Symbol) }
    rescue NameError
      nil
    end

    # The literal keys a local hash is given anywhere in `scope`: `options[:label] = x`.
    def local_index_keys(scope, name)
      @local_index_keys ||= {}.compare_by_identity
      keys = @local_index_keys[scope] ||= begin
        found = Hash.new { |hash, key| hash[key] = [] }
        stack = [scope]
        until stack.empty?
          node = stack.pop
          next if node.is_a?(Prism::DefNode) && !node.equal?(scope)

          if node.is_a?(Prism::CallNode) && node.name == :[]= && node.receiver.is_a?(Prism::LocalVariableReadNode)
            key = literal(node.arguments&.arguments&.first)
            found[node.receiver.name] << key if key
          end
          stack.concat(node.compact_child_nodes)
        end
        found
      end
      keys.fetch(name, [])
    end

    # A render call's arguments: [first positional argument, { option => value node }].
    def render_arguments(call)
      arguments = call.arguments&.arguments || []
      first = arguments.first
      options = {}
      arguments.each do |argument|
        next unless argument.is_a?(Prism::KeywordHashNode) || argument.is_a?(Prism::HashNode)

        argument.elements.each do |element|
          next unless element.is_a?(Prism::AssocNode)

          key = literal(element.key)
          options[key.to_sym] = element.value if key
        end
      end
      first = nil if first.is_a?(Prism::KeywordHashNode) || first.is_a?(Prism::HashNode)
      [first, options]
    end

    # The partial names of the renders in a view that the plugin turns into calls (`PartialRenders` in ErbViews.cc):
    # `render 'row'` or `render('row', { a: 1 })` with symbol keys, and `render partial: 'row'` with only `locals:`
    # (a hash with symbol keys) besides. A receiver other than self, a block, a `**` or a key that isn't a symbol
    # leaves a render as it is.
    def partial_render_names(node)
      names = []
      stack = [node]
      until stack.empty?
        current = stack.pop
        stack.concat(current.compact_child_nodes)
        name = current.is_a?(Prism::CallNode) && current.name == :render && partial_render_name(current)
        names << name if name
      end
      names.uniq.sort
    end

    def partial_render_name(call)
      return nil unless call.receiver.nil? || call.receiver.is_a?(Prism::SelfNode)
      return nil if call.block

      arguments = call.arguments&.arguments || []
      keywords = arguments.last.is_a?(Prism::KeywordHashNode) ? arguments.last : nil
      positional = keywords ? arguments[0...-1] : arguments
      # Sorbet reads trailing pairs with any key that isn't a symbol (`'a' => 1`) as a positional hash.
      return nil unless keywords.nil? || keywords.elements.all? { |e| e.is_a?(Prism::AssocNode) && e.key.is_a?(Prism::SymbolNode) }

      if [1, 2].include?(positional.size)
        return nil unless positional.first.is_a?(Prism::StringNode)
        return nil if positional.size == 2 && (keywords || !symbol_hash?(positional[1]))

        positional.first.unescaped
      elsif positional.empty? && keywords
        options = keywords.elements.to_h { |element| [element.key.is_a?(Prism::SymbolNode) ? element.key.unescaped : nil, element.value] }
        return nil unless options.keys.all? { |key| %w[partial locals].include?(key) } && options['partial'].is_a?(Prism::StringNode)
        return nil if options.key?('locals') && !symbol_hash?(options['locals'])

        options['partial'].unescaped
      end
    end

    def symbol_hash?(node) = node.is_a?(Prism::HashNode) && node.elements.all? { |e| e.is_a?(Prism::AssocNode) && e.key.is_a?(Prism::SymbolNode) }

    # The DefNode for each `def` in a file, by its first line.
    def defs_by_line(path) = nodes_by_line(path)[:defs]

    # The block (or lambda) that starts on each line of a file: a method defined with `define_method`, or one of
    # ActiveAdmin's actions, has its block's line as its source location.
    def blocks_by_line(path) = nodes_by_line(path)[:blocks]

    def nodes_by_line(path)
      @nodes_by_line ||= {}
      @nodes_by_line[path] ||= begin
        found = { defs: {}, blocks: {} }
        stack = [Prism.parse_file(path).value]
        until stack.empty?
          node = stack.pop
          found[:defs][node.location.start_line] = node if node.is_a?(Prism::DefNode)
          found[:blocks][node.location.start_line] ||= node if node.is_a?(Prism::BlockNode) || node.is_a?(Prism::LambdaNode)
          stack.concat(node.compact_child_nodes)
        end
        found
      end
    end
  end
end
