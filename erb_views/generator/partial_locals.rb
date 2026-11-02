# frozen_string_literal: true

require 'prism'
require_relative 'template_ruby'

module ErbViews
  # A template's strict locals, as the plugin reads them: the parameters of its first `<%# locals: (...) %>` comment
  # (Rails' strict locals), typed by its `<%# sig { params(...) } %>` comment. Rails passes locals as keywords, so
  # only keyword parameters count. `rest`: a `**` parameter's name (`:**` for an anonymous one).
  class PartialLocals
    Param = Struct.new(:name, :required, :type)

    # The name the RBI gives an anonymous `**`.
    ANONYMOUS_REST = '__rest'

    attr_reader :params, :rest, :rest_type

    # nil for a template without a locals comment: it takes any locals.
    def self.read(source)
      statements = Prism.parse(TemplateRuby.call(source)).value.statements.body
      lambda = statements.find { |node| node.is_a?(Prism::LocalVariableWriteNode) && node.name == :__locals && node.value.is_a?(Prism::LambdaNode) }
      return nil if lambda.nil?

      new(lambda.value.parameters&.parameters, sig_types(statements))
    end

    # name => the type's source, from the top-level `sig { params(...) }` statements.
    def self.sig_types(statements)
      statements.each_with_object({}) do |node, types|
        next unless node.is_a?(Prism::CallNode) && node.name == :sig && node.receiver.nil? && node.block.is_a?(Prism::BlockNode)

        params = node.block.body&.body&.first
        next unless params.is_a?(Prism::CallNode) && params.name == :params

        (params.arguments&.arguments || []).grep(Prism::KeywordHashNode).flat_map(&:elements).grep(Prism::AssocNode).each do |element|
          types[element.key.unescaped] = element.value.slice if element.key.is_a?(Prism::SymbolNode)
        end
      end
    end

    def initialize(parameters, types)
      keywords = parameters&.keywords || []
      @params = keywords.map do |param|
        name = param.name.to_s
        Param.new(name, param.is_a?(Prism::RequiredKeywordParameterNode), types[name])
      end
      rest = parameters&.keyword_rest
      if rest.is_a?(Prism::KeywordRestParameterNode)
        @rest = rest.name&.to_s || ANONYMOUS_REST
        @rest_type = rest.name && types[rest.name.to_s]
      end
    end

    def names = params.map(&:name)

    # The RBI for the method a view's render of the template calls: its locals as keyword parameters, with their
    # types (`T.untyped` without one).
    def method_rbi(method)
      sig_params = params.map { |param| "#{param.name}: #{param.type || 'T.untyped'}" }
      definitions = params.map { |param| param.required ? "#{param.name}:" : "#{param.name}: T.unsafe(nil)" }
      if rest
        sig_params << "#{rest}: #{rest_type || 'T.untyped'}"
        definitions << "**#{rest}"
      end
      sig = sig_params.empty? ? 'sig { returns(T.untyped) }' : "sig { params(#{sig_params.join(', ')}).returns(T.untyped) }"
      "  #{sig}\n  def #{method}#{"(#{definitions.join(', ')})" unless definitions.empty?}; end"
    end

    # The method for a template without a locals comment, or one the name can't pin down: any locals.
    def self.untyped_method_rbi(method) = "  sig { params(locals: T.untyped).returns(T.untyped) }\n  def #{method}(**locals); end"
  end
end
