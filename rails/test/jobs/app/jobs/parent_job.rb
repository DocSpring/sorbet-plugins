# typed: true
class ParentJob
  include Sidekiq::Job
  extend T::Sig

  sig { params(id: Integer).void }
  def perform(id); end
end
