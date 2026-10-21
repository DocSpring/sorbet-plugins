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
  end
end

module FactoryBot
end
