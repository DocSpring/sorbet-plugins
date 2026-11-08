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
end

ApplicationRecord.where # error: Method `where` does not exist on `T.class_of(ApplicationRecord)`
