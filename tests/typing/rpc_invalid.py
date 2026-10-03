from designer_sdk import App


def mistakes(app: App) -> None:
    app.add_mate(a={}, b={}, rotation_deg="90")  # type-error
    app.add_mate(b={})  # type-error
    app.add_mate(a={}, b={}, derive="exact")  # type-error
    app.remove_mate(id="3")  # type-error
    app.get_state(prat="Bracket")  # type-error
    app.measure(a={}, kind="far")  # type-error
    app.set_variable(name="w", expression=True, scope="global")  # type-error
    app.add_feature(feature={}, expect_revision={"version": 1})  # type-error
    app.list_parts(part=1)  # type-error
