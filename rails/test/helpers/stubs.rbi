# typed: true
module ActionController
  class Base
    def self.helper(*args, &block); end
    def self.helper_method(*names); end
  end
  class API; end
end
module ActionMailer
  class Base
    def self.helper(*args, &block); end
    def self.helper_method(*names); end
  end
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
end
