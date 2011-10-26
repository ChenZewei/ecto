#include <ecto/log.hpp>
#include <ecto/strand.hpp>
#include <ecto/atomic.hpp>
#include <ecto/cell.hpp>
#include <boost/unordered_map.hpp>
#include <boost/asio.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/function.hpp>

namespace ecto {

  struct strand::impl : boost::noncopyable
  {
    boost::scoped_ptr<boost::asio::io_service::strand> asio_strand_p;
  };

  strand::strand() : impl_(new impl)
  {
    ECTO_LOG_DEBUG("Created strand with id %p", id());
  }

  std::size_t strand::id() const
  {
    return reinterpret_cast<std::size_t>(impl_.get());
  }

  void strand::reset()
  {
    ECTO_LOG_DEBUG("Resetting strand %p", impl_->asio_strand_p.get());
    impl_->asio_strand_p.reset();
  }

  bool operator==(const strand& lhs, const strand& rhs)
  {
    return lhs.id() == rhs.id();
  }

  std::size_t strand_hash::operator()(const strand& s) const
  {
    return s.id();
  }

  void on_strand(cell_ptr c, boost::asio::io_service& serv, boost::function<void()> h)
  {
    ECTO_LOG_DEBUG("on_strand %s, serv=%p", c->name() % &serv);
    if (c->strand_) {
      ECTO_LOG_DEBUG("Yup %s should have a strand", c->name());
      //strands_t::scoped_lock l(strands());

      //      const ecto::strand& skey = *(c->strand_);
      //      ECTO_LOG_DEBUG("skey @ %p", &skey);
      //      boost::shared_ptr<boost::asio::io_service::strand>& strand_p = l.value[skey];
      boost::scoped_ptr<boost::asio::io_service::strand>& thestrand = c->strand_->impl_->asio_strand_p;
      if (!thestrand) {
          thestrand.reset(new boost::asio::io_service::strand(serv));
          ECTO_LOG_DEBUG("%s: Allocated new asio::strand @ %p assoc with serv @ %p",
                         c->name() % thestrand.get() % &thestrand->get_io_service());
        }
      else
        {
          boost::asio::io_service& serv_inside_strand = thestrand->get_io_service();
          ECTO_LOG_DEBUG("strand matches, %p ??? %p", &serv_inside_strand % &serv);
          ECTO_ASSERT(&serv_inside_strand == &serv,
                      "Hmm, this strand thinks it should be on a different io_service");
        }
      ECTO_LOG_DEBUG("%s: POST via strand id=%p post to serv %p", c->name() % c->strand_->id() % thestrand.get());
      thestrand->post(h);
    } else {
      ECTO_LOG_DEBUG("%s: POST (strandless) post to serv %p", c->name() % &serv);
      serv.post(h);
    }
  }
}
