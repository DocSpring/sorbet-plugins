# typed: true
# A subclass: its parent's slots by inheritance, and its own.
module DS
  class WideCardComponent < CardComponent
    renders_one :aside
  end
end

T.reveal_type(DS::WideCardComponent.new.footer) # error: Revealed type: `T.untyped`
T.reveal_type(DS::WideCardComponent.new.with_aside) # error: Revealed type: `T.untyped`
DS::CardComponent.new.aside # error: Method `aside` does not exist on `DS::CardComponent`
