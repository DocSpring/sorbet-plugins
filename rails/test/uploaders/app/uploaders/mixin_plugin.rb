# typed: true
module MixinPlugin
  def self.configure(uploader, **)
    uploader.const_get(:Attacher).include(Comparable)
  end
end
