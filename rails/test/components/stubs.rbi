# typed: true
module ViewComponent
  class Base
    def self.renders_one(slot_name, callable = nil); end
    def self.renders_many(slot_name, callable = nil); end
    def self.with_options(options, &block); end
  end
end

module ActiveSupport
  module Inflector
    def self.inflections(locale = :en, &block); end
  end
end
