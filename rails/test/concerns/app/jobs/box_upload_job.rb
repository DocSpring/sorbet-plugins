# typed: true
class BoxUploadJob
  include StorageProviderUpload
end

T.reveal_type(BoxUploadJob.provider_name) # error: Revealed type: `T.untyped`
T.reveal_type(BoxUploadJob.upload_kind) # error: Revealed type: `T.untyped`
T.reveal_type(BoxUploadJob.new.upload_kind) # error: Revealed type: `T.untyped`
