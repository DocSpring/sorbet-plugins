# typed: true
module OAuth
  # A gem's controller as the parent: its helpers come from the generated RBI, the class's own from here.
  class AuthorizationsController < Doorkeeper::AuthorizationsController
    extend T::Sig

    helper_method :oauth_application

    sig { returns(T.nilable(String)) }
    def oauth_application = nil
  end
end
