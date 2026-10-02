#pragma once

// A Template instance (docs/modeling/templates.md): ONE feature in the tree
// that expands, at every regeneration, into the features of the template it
// names, with its own values for the template's parameters and its own
// reference to stand on.
//
// It carries the DEFINITION it expands -- the template as it was at the
// revision it records -- the way a Derived carries its snapshot, so a part
// regenerates from its own file alone and nothing changes because a
// template in the library did. When the library's revision moves on, the
// instance reads "template changed" until it is updated (one undo step).
//
// Its expanded features have document ids of their OWN, minted once when it
// is placed and stored here (`ids`, template-local id -> document id), so the
// faces they make are named as any feature's are -- `<id>/g1`, `<id>/cap1`
// -- and those names are this instance's alone: two instances never
// collide, and an update keeps every name whose template feature it keeps.
// Unlinking hands the same features, with the same ids, to the tree.

#include "core/Feature.h"
#include "core/Template.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class TemplateFeature : public Feature {
public:
  explicit TemplateFeature(const std::string& name) { m_name = name; }
  // Everything but the expansion, which the next Execute rebuilds (the
  // features are owning pointers, so the compiler cannot write this one).
  TemplateFeature(const TemplateFeature& other)
      : Feature(other), templateName(other.templateName),
        definition(other.definition), values(other.values), ids(other.ids) {}

  std::string TypeName() const override { return "Template"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<TemplateFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // Its numeric parameters' values: evaluated against the document's
  // variables before it executes, so they are in its JSON and its hash.
  void VisitExprs(const ExprVisitor& visit) override;

  // What its reference parameters name: the face, plane or frame it stands
  // on. Its own expanded ids are OWNED, not referenced.
  void VisitFeatureRefs(const RefVisitor& visit) const override;
  void VisitOwnedIds(const RefVisitor& visit) const override;

  std::string WhyIncomplete() const override;

  // The document id of template feature `local`, 0 when it has none (an
  // instance placed before an update added that feature).
  uint64_t IdOf(uint64_t local) const;
  // Mints an id through `allocate` for every template feature without one,
  // and drops the ids of features the definition no longer has.
  void AssignIds(const std::function<uint64_t()>& allocate);

  // Moves the instance to `next`, a newer revision of its template
  // (docs/modeling/templates.md#revisions): every parameter `next` still
  // declares with the same kind keeps its value, `extra` gives the ones it
  // added (or overrides), and ids are minted for features it added while
  // every feature it kept keeps its id -- and so its face names. Returns why
  // not (a parameter left without a value), changing nothing.
  [[nodiscard]] std::string UpdateTo(const tmpl::Template& next,
                                     const tmpl::Values& extra,
                                     const std::function<uint64_t()>& allocate);

  // The features the last Execute expanded, in order: what get_state lists
  // under the instance and what unlink hands to the tree. Empty before the
  // first Execute, and after a failed one.
  const std::vector<std::unique_ptr<Feature>>& Expanded() const {
    return m_expanded;
  }

  // What it expands into, read off the definition: each template feature's
  // document id, type and name, [{id, type, name}]. Unlike Expanded() it
  // needs no Execute -- a part opened from baked geometry executes nothing,
  // and its instances still say what they are made of.
  nlohmann::json ExpandedRows() const;

  // The features this instance would hand the tree on unlink: built from
  // the last expansion, with each expression the template's parameters
  // replaced by the instance's values (tmpl::SubstituteParams), so the plain
  // features follow the document's variables the instance followed.
  // Refuses (empty, *why) when the instance has not built.
  std::vector<std::unique_ptr<Feature>> Unlinked(std::string* why) const;

  std::string templateName;  // the library template it was placed from
  tmpl::Template definition; // what it expands (and its revision)
  tmpl::Values values;       // by parameter name
  std::vector<std::pair<uint64_t, uint64_t>> ids; // local -> document id

private:
  std::vector<std::unique_ptr<Feature>> m_expanded;
};
