# typed: true
# The global plugins: every attachment has the attachment methods they define, with those of its uploader's.
Shrine.plugin :activerecord
Shrine.plugin :cached_attachment_data if ENV['CACHE']
