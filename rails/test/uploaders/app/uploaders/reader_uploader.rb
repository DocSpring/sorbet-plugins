# typed: true
class ReaderUploader < Shrine
  plugin ReaderPlugin # error: can't read this `plugin`: doesn't type the attachment readers of an uploader whose plugin `ReaderPlugin` defines `Attacher#get`
end
