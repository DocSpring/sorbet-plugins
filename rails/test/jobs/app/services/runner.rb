# typed: true
# Has a perform, but isn't in app/jobs.
class Runner
  include Sidekiq::Job

  def perform; end
end

Runner.perform_async # error: Method `perform_async` does not exist on `T.class_of(Runner)`
