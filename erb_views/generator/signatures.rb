# frozen_string_literal: true

module ErbViews
  # RBI for a method the generator declares itself, such as a mailer's `helper_method`s (Tapioca's HelperProxy covers
  # only controllers'): its sig when it has one, or untyped parameters from its definition.
  module Signatures
    module_function

    def method_rbi(owner, name, indent: '  ')
      method = owner.instance_method(name)
      signature = defined?(T::Utils) ? T::Utils.signature_for_method(method) : nil
      parameters = (signature&.method || method).parameters
      types = signature ? signature_types(signature) : {}
      names = parameters.each_with_index.map { |(_kind, parameter), index| parameter || :"arg#{index}" }

      typed = parameters.each_index.map do |index|
        "#{names[index]}: #{types.fetch(parameters[index][1], 'T.untyped')}"
      end
      returns = signature ? returns_text(signature) : 'returns(T.untyped)'
      sig = typed.empty? ? "sig { #{returns} }" : "sig { params(#{typed.join(', ')}).#{returns} }"
      "#{indent}#{sig}\n#{indent}def #{name}(#{parameter_list(parameters, names)}); end"
    end

    def signature_types(signature)
      types = signature.arg_types.to_h.merge(signature.kwarg_types)
      types[signature.rest_name] = signature.rest_type if signature.rest_name
      types[signature.keyrest_name] = signature.keyrest_type if signature.keyrest_name
      types[signature.block_name] = signature.block_type if signature.block_name
      types.transform_values { |type| Types.qualify(type.to_s) }
    end

    def returns_text(signature)
      return 'void' if signature.return_type.is_a?(T::Private::Types::Void)

      "returns(#{Types.qualify(signature.return_type.to_s)})"
    end

    def parameter_list(parameters, names)
      parameters.each_with_index.map do |(kind, _), index|
        name = names[index]
        case kind
        when :req then name.to_s
        when :opt then "#{name} = T.unsafe(nil)"
        when :rest then "*#{name}"
        when :keyreq then "#{name}:"
        when :key then "#{name}: T.unsafe(nil)"
        when :keyrest then "**#{name}"
        when :block then "&#{name}"
        else raise "unknown parameter kind #{kind.inspect} for #{name}"
        end
      end.join(', ')
    end
  end
end
