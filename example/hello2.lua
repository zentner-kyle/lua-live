world = ' World'

function main()
  while true do
    print('Hello' .. world)
  end
end

live.patch('main', main)

live.start(main)
