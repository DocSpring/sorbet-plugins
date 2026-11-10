# typed: true
# perform's sig names a type alias it finds through an included module.
class TypedParentJob
  include Sidekiq::Job
  include JobTypes
  extend T::Sig

  sig { params(id: RecordId).void }
  def perform(id); end
end
