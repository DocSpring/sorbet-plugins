# typed: true
module OAuth
  # A gem's controller whose name says Controller, but which is an ActionController::API: no helpers.
  class TokensController < Doorkeeper::TokensController
  end

  class MetadataController < Doorkeeper::MetadataController
  end
end

OAuth::TokensController.new.helpers # error: Method `helpers` does not exist on `OAuth::TokensController`
OAuth::MetadataController::HelperMethods # error: Unable to resolve constant `HelperMethods`
