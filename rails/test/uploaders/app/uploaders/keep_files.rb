# typed: true
# An app plugin: the plugin reads its modules from this file, and the plugins its `load_dependencies` loads.
module KeepFiles
  def self.load_dependencies(uploader, **)
    uploader.plugin :pretty_location
  end

  module AttacherMethods
    def keep_files; end
  end
end
