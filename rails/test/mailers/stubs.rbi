# typed: true
module ActionMailer
  class Base
    def self.helper(*args, &block); end
    def self.helper_method(*names); end
  end
  class MessageDelivery; end
end
module ActionView
  module Helpers
    module NumberHelper
      def number_to_currency(number, options = {}); end
    end
  end
end
module Devise
  # A gem's mailer: its actions come from Tapioca's RBI for it.
  class Mailer < ActionMailer::Base
    def self.confirmation_instructions(record, token, opts = {}); end
  end
end
class User; end
