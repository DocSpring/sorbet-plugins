# typed: true
# A concern's associations: Rails looks their classes up from the class that includes it.
module HasAuthor
  extend ActiveSupport::Concern

  included do
    T.bind(self, T.class_of(ApplicationRecord))
    belongs_to :author, class_name: 'Person'
  end
end
