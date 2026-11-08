# typed: true
class SimpleJob
  include Sidekiq::Job
  extend T::Sig

  sig { params(name: String, count: Integer, extra: T.nilable(T::Hash[String, T.untyped])).void }
  def perform(name, count, extra = nil); end
end

T.reveal_type(SimpleJob.perform_async('a', 1)) # error: Revealed type: `String`
SimpleJob.perform_async(1, 1) # error: Expected `String` but found `Integer(1)` for argument `name`
SimpleJob.perform_async('a', 1, { 'k' => 1 })
T.reveal_type(SimpleJob.perform_in(5, 'a', 1)) # error: Revealed type: `String`
SimpleJob.perform_in('soon', 'a', 1) # error: Expected `T.any(Numeric, ActiveSupport::Duration)` but found `String("soon")` for argument `interval`
T.reveal_type(SimpleJob.perform_at(Time.now, 'a', 1)) # error: Revealed type: `String`
SimpleJob.perform_at(5, 'a', 1) # error: Expected `T.any(DateTime, Time, ActiveSupport::TimeWithZone)` but found `Integer(5)` for argument `interval`
