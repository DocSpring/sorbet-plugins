# typed: true
module IntegrationUpload
  extend ActiveSupport::Concern

  included do
    T.bind(self, T::Class[T.anything])
    class_attribute :upload_kind
  end

  class_methods do
    def provider_name; end
  end
end
