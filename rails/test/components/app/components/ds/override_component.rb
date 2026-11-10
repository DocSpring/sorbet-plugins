# typed: true
# Overrides its own slot's getter and calls `super`: ViewComponent defines the getter in GeneratedSlotMethods.
module DS
  class OverrideComponent < BaseComponent
    extend T::Sig

    renders_one :header

    sig { returns(T.untyped) }
    def header
      super || 'default'
    end

    sig { returns(T::Boolean) }
    def header?
      !!super
    end
  end
end

T.reveal_type(DS::OverrideComponent.new.header) # error: Revealed type: `T.untyped`
T.reveal_type(DS::OverrideComponent.new.with_header('x')) # error: Revealed type: `T.untyped`
