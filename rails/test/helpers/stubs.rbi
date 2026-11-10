# typed: true
module ActionController
  class Base
    def self.helper(*args, &block); end
    def self.helper_method(*names); end
    def self.clear_helpers; end

    # The gem RBI's helper module: what gems add to every controller's helpers.
    module HelperMethods
      def notice; end
    end
  end
  class API; end
end
module ActionMailer
  class Base
    def self.helper(*args, &block); end
    def self.helper_method(*names); end
  end
  class MessageDelivery; end
end
module ActionView
  class Base
    def content_tag(name); end
  end
end
module ActiveSupport
  module Concern
    def included(base = nil, &block); end
  end
end
module Doorkeeper
  class AuthorizationsController < ActionController::Base; end
  # Doorkeeper's token endpoints are ActionController::API controllers, with no helpers.
  class ApplicationMetalController < ActionController::API; end
  class TokensController < ApplicationMetalController; end
  class TokenInfoController < ApplicationMetalController; end
  class MetadataController < ApplicationMetalController; end
end
