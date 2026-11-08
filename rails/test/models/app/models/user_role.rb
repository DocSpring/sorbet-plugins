# typed: true
# A table without a primary key.
class UserRole < ApplicationRecord
  self.table_name = 'users_roles'
end

T.reveal_type(UserRole.ids) # error: Revealed type: `T::Array[T.untyped]`
# Without a primary key, `id` is untyped and Rails adds no `id_value`.
T.reveal_type(UserRole.new.id) # error: Revealed type: `T.untyped`
T.reveal_type(UserRole.new.role_id) # error: Revealed type: `T.nilable(Integer)`
UserRole.new.id_value # error: Method `id_value` does not exist on `UserRole`
