# typed: true
# An attribute macro in a concern's `class_methods` block.
module HasLabels
  extend ActiveSupport::Concern

  class_methods do
    def labelled(*names)
      T.bind(self, T.class_of(ApplicationRecord))
      names.each do |name|
        attribute name, :string
      end
    end
  end
end
