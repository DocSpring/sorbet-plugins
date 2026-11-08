# typed: true
# Has a perform, but isn't in app/jobs.
class Runner
  include Sidekiq::Job

  def perform; end
end

# Sidekiq's own, untyped.
T.reveal_type(Runner.perform_async) # error: Revealed type: `T.untyped`
