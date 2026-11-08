# typed: true
# Four steps to ApplicationJob: BatchBaseJob, Export::BaseJob, DataMigrations::BaseJob, ApplicationJob.
module DataMigrations
  module Export
    class RowsBatchJob < BatchBaseJob
      extend T::Sig

      sig { params(batch_id: Integer, block: T.nilable(T.proc.void)).returns(Integer) }
      def perform(batch_id, &block)
        batch_id
      end
    end
  end
end

T.reveal_type(DataMigrations::Export::RowsBatchJob.perform_later(1)) # error: Revealed type: `T.any(DataMigrations::Export::RowsBatchJob, FalseClass)`
DataMigrations::Export::RowsBatchJob.perform_later(1) { |job| T.reveal_type(job) } # error: Revealed type: `DataMigrations::Export::RowsBatchJob`
T.reveal_type(DataMigrations::Export::RowsBatchJob.perform_now(1)) # error: Revealed type: `Integer`
DataMigrations::Export::RowsBatchJob.perform_async(1) # error: Method `perform_async` does not exist on `T.class_of(DataMigrations::Export::RowsBatchJob)`
