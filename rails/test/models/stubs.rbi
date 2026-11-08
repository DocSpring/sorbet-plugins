# typed: true
module ActiveRecord
  class Base
    def self.primary_abstract_class; end
    def self.abstract_class=(value); end
    def self.table_name=(name); end
    def self.primary_key=(name); end
    def self.scope(name, body, &block); end
    def self.enum(name, values = nil, **options); end
    def self.store_accessor(store_attribute, *keys, prefix: nil, suffix: nil); end
    def self.attribute(name, cast_type = nil, **options); end
    def self.validates(*attributes); end
    def self.with_options(options, &block); end
  end

  class Relation
    extend T::Generic
    Elem = type_member(:out) { { fixed: T.untyped } }
  end

  class AssociationRelation < Relation
    Elem = type_member(:out) { { fixed: T.untyped } }
  end

  module Associations
    class CollectionProxy < Relation
      Elem = type_member(:out) { { fixed: T.untyped } }
    end
  end

  module Batches
    class BatchEnumerator; end
  end

  module Type
    class Time
      class Value; end
    end

    class Binary
      class Data; end
    end
  end
end

module ActiveSupport
  module Concern
    def included(base = nil, &block); end
  end

  module Inflector
    def self.inflections(locale = :en, &block); end
  end

  module Multibyte
    class Chars; end
  end

  class Duration; end
end

module Audited
  class Audit < ActiveRecord::Base; end
end
