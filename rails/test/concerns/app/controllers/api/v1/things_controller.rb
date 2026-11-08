# typed: true
module API
  module V1
    class ThingsController
      include Concerns::HasIdempotencyKeys
    end
  end
end

T.reveal_type(API::V1::ThingsController.idempotent_creates) # error: Revealed type: `T.untyped`
API::V1::ThingsController.idempotent_creates = {}
API::V1::ThingsController.idempotent_creates?
API::V1::ThingsController.new.idempotent_creates # error: Method `idempotent_creates` does not exist on `API::V1::ThingsController`
