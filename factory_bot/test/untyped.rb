# typed: false

# The plugin leaves `typed: false` files alone, so their factory calls don't need the generated RBI.
RSpec.describe 'untyped' do
  it 'works' do
    create(:anything, :goes, whatever: 1)
  end
end
