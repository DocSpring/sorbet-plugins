# typed: true
# sorbet-args: --enable-experimental-rspec

RSpec.describe 'Templates API' do
  path '/templates/{id}' do
    parameter name: :id, in: :path, type: :string

    get 'Get a template' do
      produces 'application/json'

      response '200', 'found' do
        let(:id) { 'tpl_123' }

        run_test! do |data|
          expect(data).to eq(id)
          expect(response).to eq(nil)
          respnse # error: Method `respnse` does not exist
        end
      end

      response '404', 'not found' do
        run_test!
      end
    end
  end

  def path(value); end # an ordinary method with the same name is left alone

  it 'calls it' do
    path('/x')
  end
end
