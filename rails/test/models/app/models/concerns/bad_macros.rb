# typed: true
# A concern with a macro the plugin can't read: the error is on the class that includes it.
module BadMacros
  extend ActiveSupport::Concern

  included do
    T.bind(self, T.class_of(ApplicationRecord))
    name = 'x'
    scope name, -> {}
  end
end
