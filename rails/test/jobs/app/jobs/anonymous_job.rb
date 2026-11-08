# typed: true
# Anonymous parameters are copied under the names Sorbet gives them, which the sig uses too.
class AnonymousJob
  include Sidekiq::Job
  extend T::Sig

  sig { params(id: Integer, "*": String, "**": Integer).void }
  def perform(id, *, **); end
end

T.reveal_type(AnonymousJob.perform_async(1, 'a', 'b', key: 2)) # error: Revealed type: `String`
AnonymousJob.perform_async(1, 2) # error: Expected `String` but found `Integer(2)` for argument `*`
