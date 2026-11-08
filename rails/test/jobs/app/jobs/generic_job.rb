# typed: true
class GenericJob # error: rails plugin: can't copy the sig of GenericJob#perform
  include Sidekiq::Job
  extend T::Sig

  sig { type_parameters(:U).params(value: T.type_parameter(:U)).void }
  def perform(value); end
end
