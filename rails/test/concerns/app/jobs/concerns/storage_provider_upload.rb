# typed: true
# A concern that includes another: a class that includes this one gets that one's class methods too.
module StorageProviderUpload
  extend ActiveSupport::Concern
  include IntegrationUpload
end
