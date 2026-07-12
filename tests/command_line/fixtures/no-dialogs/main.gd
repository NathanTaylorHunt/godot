extends SceneTree

func _initialize() -> void:
	OS.alert("headless alert fixture", "Headless No Dialogs")
	quit(23)
