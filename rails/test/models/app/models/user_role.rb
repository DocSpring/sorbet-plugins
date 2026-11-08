# typed: true
# A table without a primary key.
class UserRole < ApplicationRecord
  self.table_name = 'users_roles'
end

T.reveal_type(UserRole.ids) # error: Revealed type: `T::Array[T.untyped]`
