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
    def self.serialize(attr_name, coder: nil, type: Object, **options); end
    def self.alias_attribute(new_name, old_name); end
    def self.ignored_columns; end
    def self.ignored_columns=(columns); end
    def self.define_method(name, &block); end
    def self.belongs_to(name, scope = nil, **options, &extension); end
    def self.has_one(name, scope = nil, **options, &extension); end
    def self.has_many(name, scope = nil, **options, &extension); end
    def self.has_and_belongs_to_many(name, scope = nil, **options, &extension); end
    def self.accepts_nested_attributes_for(*attr_names); end
    def [](name); end
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
  class TimeWithZone; end
end

# Shrine 3.10's base classes and the plugin modules the fixtures load, each with a method to call.
class Shrine
  def self.plugin(plugin, *args, **options, &block); end
  def self.attachment(name, **options); end
  def self.Attachment(name, **options); end
  def self.opts; end

  class Attacher; end
  class UploadedFile; end

  module Plugins
    module Activerecord
      module AttacherMethods; def activerecord_after_save; end; end
      module AttachmentMethods; end
    end

    module CachedAttachmentData
      module AttacherMethods; def cached_data; end; end
      module AttachmentMethods; end
    end

    # Derivatives loads it first: its `url` comes after this one.
    module DefaultUrl
      module AttacherMethods
        sig { params(options: T.untyped).returns(Integer) }
        def url(**options); end
      end

      module AttacherClassMethods; def default_url(&block); end; end
    end

    module Derivatives
      module ClassMethods; def derivatives_processor(name = nil); end; end

      module AttacherMethods
        def derivatives; end

        sig { params(path: T.untyped, options: T.untyped).returns(String) }
        def url(*path, **options); end
      end

      module AttacherClassMethods; def derivatives(name, &block); end; end
      module FileMethods; def derivative_url; end; end
      module AttachmentMethods; end

      module VersionsCompatibility
        def load_versions; end
      end
    end

    module PrettyLocation
      module InstanceMethods; def pretty_location(io, **options); end; end
    end

    module StoreDimensions
      module InstanceMethods; def extract_dimensions(io); end; end
      module ClassMethods; def dimensions_analyzer(name); end; end
      module FileMethods; def width; end; end
    end

    module UploadOptions
      module InstanceMethods; def upload_options; end; end
    end
  end
end
