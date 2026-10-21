# typed: true

class User
  def name; end
  def admin?; end
end

module RSpec
  module Core
    class ExampleGroup
      def expect(value = nil, &blk); end
      def eq(value); end
    end
  end
end

# What generate_rbi.rb writes for a `user` factory with an `admin` trait and a `name` attribute.
module FactoryBotTypes
  module User
    Model = T.type_alias { ::User }

    sig { params(traits: Symbol, name: T.untyped, blk: T.nilable(T.proc.params(arg0: Model).void)).returns(Model) }
    def self.create(*traits, name: T.unsafe(nil), &blk); end

    sig { params(traits: Symbol, name: T.untyped, blk: T.nilable(T.proc.params(arg0: Model).void)).returns(Model) }
    def self.build(*traits, name: T.unsafe(nil), &blk); end

    sig do
      params(count: Integer, traits: Symbol, name: T.untyped, blk: T.nilable(T.proc.params(arg0: Model, arg1: Integer).void))
        .returns(T::Array[Model])
    end
    def self.create_list(count, *traits, name: T.unsafe(nil), &blk); end

    sig { params(traits: Symbol, name: T.untyped).returns(T::Hash[Symbol, T.untyped]) }
    def self.attributes_for(*traits, name: T.unsafe(nil)); end

    module Traits
      sig { returns(Symbol) }
      def self.admin; end
    end

    module Definition
      sig { params(args: T.untyped, kwargs: T.untyped, block: T.nilable(T.proc.bind(Evaluator).returns(T.untyped))).void }
      def name(*args, **kwargs, &block); end

      sig { params(args: T.untyped, kwargs: T.untyped, block: T.nilable(T.proc.bind(Evaluator).returns(T.untyped))).void }
      def nickname(*args, **kwargs, &block); end

      sig { params(args: T.untyped, kwargs: T.untyped, block: T.nilable(T.proc.bind(Evaluator).returns(T.untyped))).void }
      def admin(*args, **kwargs, &block); end

      sig { params(name: Symbol, block: T.proc.bind(Definition).void).void }
      def trait(name, &block); end

      sig { params(block: T.proc.bind(Definition).void).void }
      def transient(&block); end

      sig do
        params(
          names: Symbol,
          block: T.proc.bind(::FactoryBot::SyntaxRunner).params(record: Model, evaluator: Evaluator).void
        ).void
      end
      def after(*names, &block); end

      sig { params(name: Symbol, options: T.untyped, block: T.nilable(T.proc.void)).void }
      def factory(name, options = {}, &block); end
    end

    class Evaluator < ::User
      sig { returns(T.untyped) }
      def nickname; end
    end
  end
end

module FactoryBotTypes
  module AdminUser
    module Definition
      sig { params(args: T.untyped, kwargs: T.untyped, block: T.nilable(T.proc.bind(Evaluator).returns(T.untyped))).void }
      def admin(*args, **kwargs, &block); end
    end

    class Evaluator < ::User; end
  end
end

module FactoryBot
  def self.define(&blk); end

  module Syntax
    module Default
      class DSL
        def factory(name, options = {}, &blk); end
        def sequence(name, *args, &blk); end
      end
    end
  end

  class SyntaxRunner
    def create(*args, **kwargs, &blk); end
  end
end
