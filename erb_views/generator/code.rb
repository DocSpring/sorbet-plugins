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
    Render = Struct.new(:file, :node, :scope, :definite, :bindings, :via, :chain_writes, keyword_init: true)
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

      def visit_if_node(node)
        visit(node.predicate)
        conditional do
          visit(node.statements)
          visit(node.subsequent)
        end
      end

      def visit_unless_node(node)
        visit(node.predicate)
        conditional do
          visit(node.statements)
          visit(node.else_clause)
        end
      end

      def visit_case_node(node)
        visit(node.predicate)
        conditional do
          node.conditions.each { |condition| visit(condition) }
          visit(node.else_clause)
        end
      end

      def visit_case_match_node(node)
        visit(node.predicate)
        conditional do
          node.conditions.each { |condition| visit(condition) }
          visit(node.else_clause)
        end
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
        visit(node.expression)
        conditional { visit(node.rescue_expression) }
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
      end

      def visit_instance_variable_or_write_node(node)
        write(node.name, node, node.value)
        super
      end

      def visit_instance_variable_operator_write_node(node)
        write(node.name, node, nil)
        super
      end

      def visit_instance_variable_and_write_node(node)
        # `@a &&= x` assigns only when @a is already set.
        conditional { write(node.name, node, node.value) }
        super
      end

      def visit_instance_variable_target_node(node)
        write(node.name, node, nil) # `@a, @b = ...`
        super
      end

      def visit_call_node(node)
        if %i[[] fetch key? include?].include?(node.name) && local_assigns?(node.receiver)
          key = node.arguments&.arguments&.first
          @facts.local_keys << key.unescaped if key.is_a?(Prism::SymbolNode)
        end
        if node.receiver.nil? || node.receiver.is_a?(Prism::SelfNode)
          case node.name
          when :render
            @facts.renders << Render.new(file: @file, node:, scope: @scope, definite: @definite)
          when :instance_variable_set
            name = node.arguments&.arguments&.first
            write(name.unescaped.to_sym, node, node.arguments.arguments[1]) if name.is_a?(Prism::SymbolNode)
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

      def conditional
        definite = @definite
        @definite = false
        yield
      ensure
        @definite = definite
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

    # The `return`s in a method's body (not in a block, a lambda or another method).
    def returns(node, found = [])
      found << node if node.is_a?(Prism::ReturnNode)
      node.compact_child_nodes.each do |child|
        returns(child, found) unless child.is_a?(Prism::DefNode) || child.is_a?(Prism::BlockNode) || child.is_a?(Prism::LambdaNode)
      end
      found
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
        either(node.statements, node.subsequent.is_a?(Prism::ElseNode) ? node.subsequent.statements : node.subsequent, &self_call)
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
    # becomes UNKNOWN), the branches of an `if` or `case`, the values assigned to a local variable in `scope`, and
    # what a method returns (`method_body` gives a self call's [body, scope], or nil). nil when it isn't a string we
    # can follow, as with `render @record` or a block's parameter.
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
                 found = writes.map { |value| string_values(value, scope, depth + 1, &method_body) }
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
    # splats, a local's values, and what a lambda in a local returns (`color.call('x', a: 1)` with
    # `color = ->(key, **options) { { key:, **options } }` has key and a). Unknown splats add nothing.
    def hash_keys(node, scope, depth = 0, bindings = nil)
      return [] if node.nil? || depth > 6

      case node
      when Prism::HashNode, Prism::KeywordHashNode
        node.elements.flat_map do |element|
          case element
          when Prism::AssocNode then [literal(element.key)].compact
          when Prism::AssocSplatNode then hash_keys(element.value, scope, depth + 1, bindings)
          else []
          end
        end
      when Prism::LocalVariableReadNode
        if bindings&.key?(node.name)
          argument, caller = bindings.fetch(node.name)
          hash_keys(argument, caller, depth + 1)
        else
          local_writes(scope, node.name).flat_map { |value| hash_keys(value, scope, depth + 1, bindings) }
        end
      when Prism::ParenthesesNode then hash_keys(node.body, scope, depth + 1, bindings)
      when Prism::StatementsNode then hash_keys(node.body.last, scope, depth + 1, bindings)
      when Prism::CallNode
        if %i[call ()].include?(node.name) && node.receiver.is_a?(Prism::LocalVariableReadNode)
          local_writes(scope, node.receiver.name).flat_map { |value| lambda_keys(value, node, scope, depth) }
        else
          []
        end
      else []
      end.uniq
    end

    # The keys a lambda returns when `call` calls it: its keyword-rest parameter is the call's keywords.
    def lambda_keys(lambda, call, scope, depth)
      lambda = lambda.block if lambda.is_a?(Prism::CallNode) && lambda.name == :lambda && lambda.receiver.nil?
      return [] unless (lambda.is_a?(Prism::LambdaNode) || lambda.is_a?(Prism::BlockNode)) && lambda.body

      parameters = lambda.parameters&.parameters
      keyrest = parameters&.keyword_rest
      keywords = call.arguments&.arguments&.find { |argument| argument.is_a?(Prism::KeywordHashNode) }
      bindings = keyrest.respond_to?(:name) && keyrest.name && keywords ? { keyrest.name => [keywords, scope] } : {}
      hash_keys(lambda.body, scope, depth + 1, bindings)
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
