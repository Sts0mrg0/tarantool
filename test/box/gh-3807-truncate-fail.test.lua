test_run = require('test_run').new()

-- Creating tarantool with 32 megabytes memory to make truncate fail easier.
test_run:cmd("create server master with script='box/low_memory.lua'")
test_run:cmd('start server master')
test_run:cmd("switch master")


test_run:cmd("setopt delimiter ';'")
function create_space(name)
    local space = box.schema.create_space(name)
    space:format({
        { name = "id",  type = "unsigned" },
        { name = "val", type = "str" }
    })
    space:create_index('primary', { parts = { 'id' } })
    return space
end;

function insert(space, i)
    space:insert{ i, string.rep('-', 256) }
end;

function fill_space(space, start)
    local err = nil
    local i = start
    while err == nil do _, err = pcall(insert, space, i) i = i + 1 end
end;

-- Creating space if possible. If the space creation fails, stacking
-- some more tuples into the test space to exhaust slabs.
-- Then trying to truncate all spaces except the filled one.
-- Truncate shouldn't fail.
function stress_truncation(i)
    local res, space = pcall(create_space, 'test' .. tostring(i))
    if res then spaces[i] = space return end
    fill_space(box.space.test, box.space.test:len())
    for _, s in pairs(spaces) do s:truncate() end
end;
test_run:cmd("setopt delimiter ''");


_ = create_space('test')
fill_space(box.space.test, 0)

spaces = {}
counter = 0
status, res = true, nil
while status and counter < 42 do status, res = pcall(stress_truncation, counter) counter = counter + 1 end
status
res

-- Cleanup.
test_run:cmd('switch default')
test_run:drop_cluster({'master'})