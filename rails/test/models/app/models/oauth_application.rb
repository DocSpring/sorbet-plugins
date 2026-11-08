# typed: true
# The app's acronym: `oauth_applications`, not `o_auth_applications`. A uuid primary key.
class OAuthApplication < ApplicationRecord
end

T.reveal_type(OAuthApplication.ids) # error: Revealed type: `T::Array[String]`
