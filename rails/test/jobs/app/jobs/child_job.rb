# typed: true
# Sidekiq from its parent's file; its own perform takes a String.
class ChildJob < ParentJob
  extend T::Sig

  sig { params(uid: String).void }
  def perform(uid); end
end

ChildJob.perform_async('tpl_1')
ChildJob.perform_async(1) # error: Expected `String` but found `Integer(1)` for argument `uid`
