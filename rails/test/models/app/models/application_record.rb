# typed: true
class ApplicationRecord < ActiveRecord::Base
  primary_abstract_class
end

ApplicationRecord.where # error: Method `where` does not exist on `T.class_of(ApplicationRecord)`
