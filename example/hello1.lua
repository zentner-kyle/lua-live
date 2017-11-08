function main()
  while true do
    print('Hello')
  end
end

live.patch('main', main)

live.start(main)
