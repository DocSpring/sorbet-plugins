# typed: true
# A plain module's hook that declares an association on the class that includes it: the plugin doesn't read it.
module LegacyMacros
  def self.included(base)
    base.extend(ClassMethods)
    base.has_many :legacy_items
  end

  module ClassMethods; end
end
