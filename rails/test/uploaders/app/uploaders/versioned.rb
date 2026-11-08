# typed: true
# Its `configure` sets `opts[:versions]`: an attachment's reader can return a hash of versions.
module Versioned
  def self.configure(uploader, **opts)
    uploader.opts[:versions] ||= {}
  end

  module ClassMethods
    def version_names; end
  end

  module FileMethods
    def version_name; end
  end
end
