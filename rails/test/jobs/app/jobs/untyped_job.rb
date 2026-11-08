# typed: true
class UntypedJob
  include Sidekiq::Worker

  def perform(anything, options = {}, *rest, key:, **opts); end
end

T.reveal_type(UntypedJob.perform_async(1, {}, 2, key: 3)) # error: Revealed type: `String`
UntypedJob.perform_async # error: Not enough arguments provided for method `UntypedJob.perform_async` # error: Missing required keyword argument `key`
