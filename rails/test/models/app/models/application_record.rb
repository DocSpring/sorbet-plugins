# typed: true
class ApplicationRecord < ActiveRecord::Base
  primary_abstract_class

  # An enum macro of the app's own: the plugin reads the options it passes on to `enum` and their defaults.
  def self.attr_enum(name, values, default: 0, prefix: true, **)
    attribute(name, :integer, default:)
    enum(name, values, prefix:, **)
  end

  # One that always sets an option.
  def self.suffixed_enum(name, values)
    enum(name, values, suffix: true)
  end

  # An attribute macro of the app's own: each name it's given gets `attribute name, :boolean`.
  def self.default_boolean_value(*fields, value: false)
    fields.each do |field|
      attribute field, :boolean, default: value
      define_method(field) do
        T.bind(self, ApplicationRecord)
        self[field].nil? ? value : self[field]
      end
    end
  end

  # A macro the plugin can't read: it would have to run the code to know what it declares.
  def self.has_versions
    has_many :versions
  end

  # Nor this one.
  def self.text_attribute(name)
    attribute(:"#{name}_text", :string)
  end
end

ApplicationRecord.where # error: Method `where` does not exist on `T.class_of(ApplicationRecord)`
