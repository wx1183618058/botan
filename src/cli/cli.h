/*
* (C) 2015 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#ifndef BOTAN_CLI_H__
#define BOTAN_CLI_H__

#include <botan/build.h>
#include <botan/parsing.h>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Botan_CLI {

class CLI_Error : public std::runtime_error
   {
   public:
      CLI_Error(const std::string& s) : std::runtime_error(s) {}
   };

class CLI_IO_Error : public CLI_Error
   {
   public:
      CLI_IO_Error(const std::string& op, const std::string& who) :
         CLI_Error("Error " + op + " " + who) {}
   };

class CLI_Usage_Error : public CLI_Error
   {
   public:
      CLI_Usage_Error(const std::string& what) : CLI_Error(what) {}
   };

/* Thrown eg when a requested feature was compiled out of the library
   or is not available, eg hashing with
*/
class CLI_Error_Unsupported : public CLI_Error
   {
   public:
      CLI_Error_Unsupported(const std::string& what,
                            const std::string& who) :
         CLI_Error(what + " with '" + who + "' unsupported or not available") {}
   };

struct CLI_Error_Invalid_Spec : public CLI_Error
   {
   public:
      CLI_Error_Invalid_Spec(const std::string& spec) :
         CLI_Error("Invalid command spec '" + spec + "'") {}
   };

class Command
   {
   public:
      /**
      * The spec string specifies the format of the command line, eg for
      * a somewhat complicated command:
      * cmd_name --flag --option1= --option2=opt2val input1 input2 *rest
      *
      * By default this is the value returned by help_text()
      *
      * The first value is always the command name. Options may appear
      * in any order. Named arguments are taken from the command line
      * in the order they appear in the spec.
      *
      * --flag can optionally be specified, and takes no value.
      * Check for it in go() with flag_set()
      *
      * --option1 is an option whose default value (if the option
      * does not appear on the command line) is the empty string.
      *
      * --option2 is an option whose default value is opt2val
      * Read the values in go() using get_arg or get_arg_sz.
      *
      * The values input1 and input2 specify named arguments which must
      * be provided. They are also access via get_arg/get_arg_sz
      * Because options and arguments for a single command share the same
      * namespace you can't have a spec like:
      *   cmd --input input
      * but you hopefully didn't want to do that anyway.
      *
      * The leading '*' on '*rest' specifies that all remaining arguments
      * should be packaged in a list which is available as get_arg_list("rest").
      * This can only appear on a single value and should be the final
      * named argument.
      *
      * Every command has implicit flags --help, --verbose and implicit
      * options --output= and --error-output= which override the default
      * use of std::cout and std::cerr.
      *
      * Use of --help is captured in run() and returns help_text().
      * Use of --verbose can be checked with verbose() or flag_set("verbose")
      */
      Command(const std::string& cmd_spec) : m_spec(cmd_spec)
         {
         // for checking all spec strings at load time
         //parse_spec();
         }

      int run(const std::vector<std::string>& params)
         {
         try
            {
            // avoid parsing specs except for the command actually running
            parse_spec();

            std::vector<std::string> args;
            for(auto&& param : params)
               {
               if(param.find("--") == 0)
                  {
                  // option
                  const auto eq = param.find('=');

                  if(eq == std::string::npos)
                     {
                     const std::string opt_name = param.substr(2, std::string::npos);

                     if(m_spec_flags.count(opt_name) == 0)
                        {
                        if(m_spec_opts.count(opt_name))
                           throw CLI_Usage_Error("Invalid usage of option --" + opt_name +
                                                 " without value");
                        else
                           throw CLI_Usage_Error("Unknown flag --" + opt_name);
                        }

                     m_user_flags.insert(opt_name);
                     }
                  else
                     {
                     const std::string opt_name = param.substr(2, eq - 2);
                     const std::string opt_val = param.substr(eq + 1, std::string::npos);

                     if(m_spec_opts.count(opt_name) == 0)
                        {
                        throw CLI_Usage_Error("Unknown option --" + opt_name);
                        }

                     m_user_args.insert(std::make_pair(opt_name, opt_val));
                     }
                  }
               else
                  {
                  // argument
                  args.push_back(param);
                  }
               }

            size_t arg_i = 0;
            for(auto&& arg : m_spec_args)
               {
               if(arg_i >= args.size())
                  {
                  // not enough arguments
                  throw CLI_Usage_Error("Invalid argument count, got " +
                                        std::to_string(args.size()) +
                                        " expected " +
                                        std::to_string(m_spec_args.size()));
                  }

               m_user_args.insert(std::make_pair(arg, args[arg_i]));

               ++arg_i;
               }

            if(m_spec_rest.empty())
               {
               if(arg_i != args.size())
                  throw CLI_Usage_Error("Too many arguments");
               }
            else
               {
               m_user_rest.assign(args.begin() + arg_i, args.end());
               }

            if(flag_set("help"))
               {
               output() << help_text() << "\n";
               return 1;
               }

            if(m_user_args.count("output"))
               {
               m_output_stream.reset(new std::ofstream(get_arg("output")));
               }

            if(m_user_args.count("error_output"))
               {
               m_error_output_stream.reset(new std::ofstream(get_arg("error_output")));
               }

            // Now insert any defaults for options not supplied by the user
            for(auto&& opt : m_spec_opts)
               {
               if(m_user_args.count(opt.first) == 0)
                  {
                  m_user_args.insert(opt);
                  }
               }

            this->go();
            return 0;
            }
         catch(CLI_Usage_Error& e)
            {
            error_output() << "Usage error: " << e.what() << "\n";
            error_output() << help_text() << "\n";
            return 1;
            }
         catch(std::exception& e)
            {
            error_output() << "Error: " << e.what() << "\n";
            return 2;
            }
         catch(...)
            {
            error_output() << "Error: unknown exception\n";
            return 2;
            }
         }

      virtual std::string help_text() const
         {
         return "Usage: " + m_spec;
         }

      std::string cmd_name() const
         {
         return m_spec.substr(0, m_spec.find(' '));
         }

   protected:

      void parse_spec()
         {
         const std::vector<std::string> parts = Botan::split_on(m_spec, ' ');

         if(parts.size() == 0)
            throw CLI_Error_Invalid_Spec(m_spec);

         for(size_t i = 1; i != parts.size(); ++i)
            {
            const std::string s = parts[i];

            if(s.empty()) // ?!? (shouldn't happen)
               throw CLI_Error_Invalid_Spec(m_spec);

            if(s.size() > 2 && s[0] == '-' && s[1] == '-')
               {
               // option or flag

               auto eq = s.find('=');

               if(eq == std::string::npos)
                  {
                  m_spec_flags.insert(s.substr(2, std::string::npos));
                  }
               else
                  {
                  m_spec_opts.insert(std::make_pair(s.substr(2, eq - 2),
                                                    s.substr(eq + 1, std::string::npos)));
                  }
               }
            else if(s[0] == '*')
               {
               // rest argument
               if(m_spec_rest.empty() && s.size() > 2)
                  {
                  m_spec_rest = s.substr(1, std::string::npos);
                  }
               else
                  {
                  throw CLI_Error_Invalid_Spec(m_spec);
                  }
               }
            else
               {
               // named argument
               if(!m_spec_rest.empty()) // rest arg wasn't last
                  throw CLI_Error("Invalid command spec " + m_spec);

               m_spec_args.push_back(s);
               }
            }

         m_spec_flags.insert("verbose");
         m_spec_flags.insert("help");
         m_spec_opts.insert(std::make_pair("output", ""));
         m_spec_opts.insert(std::make_pair("error-output", ""));
         }

      /*
      * The actual functionality of the cli command implemented in subclas
      */
      virtual void go() = 0;

      std::ostream& output()
         {
         if(m_output_stream.get())
            return *m_output_stream;
         return std::cout;
         }

      std::ostream& error_output()
         {
         if(m_error_output_stream.get())
            return *m_error_output_stream;
         return std::cerr;
         }

      bool verbose() const
         {
         return flag_set("verbose");
         }

      bool flag_set(const std::string& flag_name) const
         {
         return m_user_flags.count(flag_name) > 0;
         }

      std::string get_arg(const std::string& opt_name) const
         {
         auto i = m_user_args.find(opt_name);
         if(i == m_user_args.end())
            {
            // this shouldn't occur unless you passed the wrong thing to get_arg
            throw CLI_Error("Unknown option " + opt_name + " used (program bug)");
            }
         return i->second;
         }

      /*
      * Like get_arg() but if the argument was not specified or is empty, returns otherwise
      */
      std::string get_arg_or(const std::string& opt_name, const std::string& otherwise) const
         {
         auto i = m_user_args.find(opt_name);
         if(i == m_user_args.end() || i->second.empty())
            {
            return otherwise;
            }
         return i->second;
         }

      size_t get_arg_sz(const std::string& opt_name) const
         {
         const std::string s = get_arg(opt_name);

         try
            {
            return static_cast<size_t>(std::stoul(s));
            }
         catch(std::exception& e)
            {
            throw CLI_Usage_Error("Invalid integer value '" + s + "' for option " + opt_name);
            }
         }

      std::vector<std::string> get_arg_list(const std::string& what) const
         {
         if(what != m_spec_rest)
            throw CLI_Error("Unexpected list name '" + what + "'");

         return m_user_rest;
         }

      /*
      * Read an entire file into memory and return the contents
      */
      std::vector<uint8_t> slurp_file(const std::string& input_file) const
         {
         std::vector<uint8_t> buf;
         auto insert_fn = [&](const uint8_t b[], size_t l)
            { buf.insert(buf.end(), b, b + l); };
         this->read_file(input_file, insert_fn);
         return buf;
         }

      std::string slurp_file_as_str(const std::string& input_file)
         {
         std::string str;
         auto insert_fn = [&](const uint8_t b[], size_t l)
            { str.append(reinterpret_cast<const char*>(b), l); };
         this->read_file(input_file, insert_fn);
         return str;
         }

      /*
      * Read a file calling consumer_fn() with the inputs
      */
      void read_file(const std::string& input_file,
                     std::function<void (uint8_t[], size_t)> consumer_fn,
                     size_t buf_size = 0) const
         {
         // Any need to support non-binary files here?
         std::ifstream in(input_file, std::ios::binary);

         // Avoid an infinite loop on --buf-size=0
         std::vector<uint8_t> buf(buf_size == 0 ? 4096 : buf_size);

         while(in.good())
            {
            in.read(reinterpret_cast<char*>(buf.data()), buf.size());
            consumer_fn(buf.data(), in.gcount());
            }
         }

      template<typename Alloc>
      void write_output_file(const std::string& who,
                             const std::vector<uint8_t, Alloc>& vec) const
         {
         write_output_file(who, vec.begin(), vec.size());
         }

      void write_output_file(const std::string& output_file,
                             const uint8_t buf[], size_t buf_len) const
         {
         std::ofstream out(output_file, std::ios::binary);
         if(!out.good())
            throw CLI_IO_Error("writing", output_file);

         out.write(reinterpret_cast<const char*>(buf), buf_len);
         out.close();
         }

      void write_output_file(const std::string& output_file, const std::string& outstr) const
         {
         std::ofstream out(output_file);
         if(!out.good())
            throw CLI_IO_Error("writing", output_file);

         out.write(outstr.data(), outstr.size());
         out.close();
         }

   private:
      // set in constructor
      std::string m_spec;

      // set in parse_spec() from m_spec
      std::vector<std::string> m_spec_args;
      std::set<std::string> m_spec_flags;
      std::map<std::string, std::string> m_spec_opts;
      std::string m_spec_rest;

      // set in run() from user args
      std::map<std::string, std::string> m_user_args;
      std::set<std::string> m_user_flags;
      std::vector<std::string> m_user_rest;

      std::unique_ptr<std::ofstream> m_output_stream;
      std::unique_ptr<std::ofstream> m_error_output_stream;


   public:
      // the registry interface:

      static std::map<std::string, std::unique_ptr<Command>>& global_registry()
         {
         static std::map<std::string, std::unique_ptr<Command>> g_cmds;
         return g_cmds;
         }

      static Command* get_cmd(const std::string& name)
         {
         auto& reg = Command::global_registry();
         auto i = reg.find(name);
         if(i == reg.end())
            {
            return nullptr;
            }

         return i->second.get();
         }

      class Registration
         {
         public:
            Registration(Command* cmd)
               {
               const std::string name = cmd->cmd_name();
               auto& reg = Command::global_registry();

               if(reg.count(name) > 0)
                  {
                  throw std::logic_error("Duplicated registration of command " + name);
                  }

               Command::global_registry().insert(std::make_pair(name, std::unique_ptr<Command>(cmd)));
               }
         };
   };

#define BOTAN_REGISTER_COMMAND(CLI_Class) \
   namespace { Botan_CLI::Command::Registration reg_cmd_ ## CLI_Class(new CLI_Class); }

}

#endif
