# typed: true
module ActiveSupport
  module Concern
    def included(base = nil, &block); end
    def class_methods(&block); end
  end
end
class Class
  def class_attribute(*attrs, instance_accessor: true, instance_reader: true, instance_writer: true, instance_predicate: true, default: nil); end
end
