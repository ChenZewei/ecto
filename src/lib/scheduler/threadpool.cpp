#include <string>
#include <map>
#include <set>
#include <utility>
#include <deque>

#include <boost/make_shared.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/unordered_map.hpp>

#include <ecto/plasm.hpp>
#include <ecto/tendril.hpp>
#include <ecto/module.hpp>
#include <ecto/log.hpp>
#include <ecto/strand.hpp>

#include <ecto/graph_types.hpp>
#include <ecto/plasm.hpp>
#include <ecto/scheduler/invoke.hpp>
#include <ecto/scheduler/threadpool.hpp>

#include <boost/spirit/home/phoenix/core.hpp>
#include <boost/spirit/home/phoenix/operator.hpp>
#include <boost/exception.hpp>

namespace ecto {

  using namespace ecto::graph;
  using boost::bind;
  using boost::ref;

  namespace scheduler {
    struct threadpool::impl 
    {
      typedef boost::function<bool(unsigned)> respawn_cb_t;

      struct invoker : boost::noncopyable
      {
        typedef boost::shared_ptr<invoker> ptr;

        threadpool::impl& context;

        boost::asio::deadline_timer dt;
        graph_t& g;
        graph_t::vertex_descriptor vd;
        unsigned n_calls;
        respawn_cb_t respawn;
        boost::mutex mtx;



        invoker(threadpool::impl& context_,
                graph_t& g_, 
                graph_t::vertex_descriptor vd_,
                respawn_cb_t respawn_)
          : context(context_), dt(context.serv), g(g_), vd(vd_), n_calls(0), respawn(respawn_)
        { }

        void async_wait_for_input()
        {
          ECTO_LOG_DEBUG("%s async_wait_for_input", this);
          boost::mutex::scoped_lock lock(mtx);
          namespace asio = boost::asio;

          // keep outer run() from returning
          asio::io_service::work work(context.serv);

          if (inputs_ready()) {
            ECTO_LOG_DEBUG("%s inputs ready", this);
            module::ptr m = g[vd];
            if (m->strand_)
              {
                const ecto::strand& skey = *(m->strand_);
                boost::shared_ptr<asio::io_service::strand>& strand_p = context.strands[skey];
                if (!strand_p) {
                  strand_p.reset(new boost::asio::io_service::strand(context.serv));
                }
                strand_p->post(bind(&invoker::invoke, this));
              }
            else
              {
                context.serv.post(bind(&invoker::invoke, this));
              }
          } else {
            ECTO_LOG_DEBUG("%s wait", this);
            dt.expires_from_now(boost::posix_time::milliseconds(1));
            dt.wait();
            context.serv.post(bind(&invoker::async_wait_for_input, this));
          }
        }

        void invoke()
        {
          ECTO_LOG_DEBUG("%s invoke", this);
          boost::mutex::scoped_lock lock(mtx);
          ecto::scheduler::invoke_process(g, vd);
          ++n_calls;
          if (respawn(n_calls)) 
            {
              context.serv.post(bind(&invoker::async_wait_for_input, this));
            }
          else
            ECTO_LOG_DEBUG("n_calls (%u) reached, no respawn", n_calls);
        }
        
        bool inputs_ready()
        {
          graph_t::in_edge_iterator in_beg, in_end;
          for (tie(in_beg, in_end) = in_edges(vd, g);
               in_beg != in_end; ++in_beg)
            {
              graph::edge::ptr e = g[*in_beg];
              if (e->size() == 0)
                return false;
            }

          graph_t::out_edge_iterator out_beg, out_end;
          for (tie(out_beg, out_end) = out_edges(vd, g);
               out_beg != out_end; ++out_beg)
            {
              graph::edge::ptr e = g[*out_beg];
              if (e->size() > 0)
                return false;
            }

          return true;
        }

        ~invoker() { ECTO_LOG_DEBUG("%s ~invoker", this); }
      }; // struct invoker

      void run_service(boost::asio::io_service& serv) 
      {
        try {
          serv.run();
        } catch (const boost::exception& e) {
          std::cout << "CAUGHT 'ER: " << boost::diagnostic_information(e) << std::endl;
          boost::lock_guard<boost::mutex> lock(exception_mtx);
          eptr = boost::current_exception();
          exception_cond.notify_all();
        } catch (const std::exception& e) {
          std::cout << "run_services caught: " << boost::diagnostic_information(e) << std::endl;
          boost::lock_guard<boost::mutex> lock(exception_mtx);
          eptr = boost::current_exception();
          std::cout << "exception set, going to notify all now\n";
          exception_cond.notify_all();
        }
      }

      void threadpool_joiner(boost::thread_group& tg) 
      {
        std::cout << "joining the threadpool..." << std::endl;
        tg.join_all();
        std::cout << "joined.  notifying..." << std::endl;
        {
          boost::mutex::scoped_lock lock(exception_mtx);
          eptr = boost::copy_exception(std::runtime_error("IS NO ERROR, EES JOINED, OKAY BOSS"));
          exception_cond.notify_all();
          std::cout << "notified\n";
        }
      }

      int execute(unsigned nthreads, impl::respawn_cb_t respawn, graph_t& graph)
      {
        namespace asio = boost::asio;

        serv.reset();

        graph_t::vertex_iterator begin, end;
        for (tie(begin, end) = vertices(graph);
             begin != end;
             ++begin)
          {
            impl::invoker::ptr ip(new impl::invoker(*this, graph, *begin, respawn));
            invokers[*begin] = ip;
            ip->async_wait_for_input();
          }


        boost::thread_group tgroup;
        { 
          asio::io_service::work work(serv);

          for (unsigned j=0; j<nthreads; ++j)
            {
              ECTO_LOG_DEBUG("%s Start thread %u", this % j);
              tgroup.create_thread(bind(&impl::run_service, this, ref(serv)));
            }
        } // let work go out of scope...   invokers now have their own work on serv
        boost::thread joiner(boost::bind(&impl::threadpool_joiner, this, boost::ref(tgroup))); 


        {
          boost::mutex::scoped_lock exceptlock(exception_mtx);
          while (eptr != boost::exception_ptr())
            {
              std::cout << "waiting...\n";
              exception_cond.wait(exceptlock);
            }
          std::cout << "rethrow time..." << std::endl;
          boost::rethrow_exception(eptr);
        }
        
        joiner.join();

        return 0;
      }

      ~impl() 
      {
        // be sure your invokers disappear before you do (you're
        // holding the main service)
        invokers_t().swap(invokers);
      }

      typedef std::map<graph_t::vertex_descriptor, invoker::ptr> invokers_t;
      invokers_t invokers;
      boost::asio::io_service serv;
      boost::unordered_map<ecto::strand, 
                           boost::shared_ptr<boost::asio::io_service::strand>,
                           ecto::strand_hash> strands;
      boost::exception_ptr eptr;
      boost::mutex exception_mtx;
      boost::condition_variable exception_cond;
    };

    threadpool::threadpool(plasm& p)
      : graph(p.graph()), impl_(new impl)
    { }

    namespace phx = boost::phoenix;

    int threadpool::execute(unsigned nthreads)
    {
      return impl_->execute(nthreads, phx::val(true), graph);
    }

    int threadpool::execute(unsigned nthreads, unsigned ncalls)
    {
      return impl_->execute(nthreads, boost::phoenix::arg_names::arg1 < ncalls, graph);
    }
  }
}
