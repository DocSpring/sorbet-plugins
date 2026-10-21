# typed: true
# sorbet-args: --enable-experimental-rspec

%w[first second].each do |name|
  title = name.capitalize

  RSpec.describe name do
    if title
      it 'sees the loop variable and locals' do
        title.anything
        get '/'
        gt '/' # error: Method `gt` does not exist
      end
    end
  end
end
