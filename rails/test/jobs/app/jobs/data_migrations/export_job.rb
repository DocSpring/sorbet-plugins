# typed: true
# An ActiveJob job whose perform it inherits gets nothing, as with Tapioca.
module DataMigrations
  class ExportJob < Export::RowsBatchJob
  end
end

DataMigrations::ExportJob.perform_now(1)
