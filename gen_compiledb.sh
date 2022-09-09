#! /bin/bash

ruby -r find -r shellwords -r stringio -e '
    cdb = StringIO.new
    cdb << "["
    Find.find "/home/chu/cheri/build/cheribsd-riscv64-hybrid-build/home/chu/cheri/cheribsd/riscv.riscv64" do |path|
        next unless FileTest.file?(path) && path.end_with?(".meta")
        args = nil
        File.readlines(path).each do |line|
            line = line.chomp
            if line =~ /^CMD.*\/clang/; args=Shellwords::split(line[4..])
            elsif line =~ /^OODATE /; file=line[7..]
                if file =~ /\.(c|cc|cpp)$/ && args
                cdb << "," if cdb.pos > 1
                cdb << %{{"directory":#{Dir.pwd.dump}, "arguments":#{args}, "file":#{file.dump}}\n}
                end
            end
        end
    end
    cdb << "]"
    print cdb.string' > compile_commands.json

