# typed: true
# `Kind` in the sig means Uploads::Kind: the copied methods keep this file's constant scope.
module Uploads
  class Kind; end

  module Shared
    extend T::Sig

    sig { params(kind: Kind).void }
    def perform(kind); end
  end
end
