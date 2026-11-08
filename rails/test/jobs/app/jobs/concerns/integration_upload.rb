# typed: true
module IntegrationUpload
  extend T::Sig

  sig { params(action_klass: String, action_id: Integer).void }
  def perform(action_klass, action_id); end
end
